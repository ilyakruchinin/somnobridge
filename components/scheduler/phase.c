/*
 * SomnoPal — phase-tracking poll schedule (update3 spec §4)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * WATCH/HUNT pull on a fixed gap. Once a transition is bracketed, the
 * tracker keeps whole [unchanged-request-start, changed-response-end]
 * intervals, assigns each to an explicit 60 s cycle, intersects bounded
 * period constraints, and arms a window around the resulting feasible
 * next-cycle interval. A miss or ambiguous cycle returns to HUNT — the
 * grid shift itself is never treated as precision.
 */
#include <math.h>
#include <string.h>

#include "phase.h"

/* -------- constants (spec §4.4 bounded attempts + guards) -------- */
#define PH_MARGIN_US       500000LL   /* window edge allowance        */
#define PH_CYC_INIT_US     300000LL   /* per-cycle variation bound J  */
#define PH_CYC_MIN_US      100000LL
#define PH_CYC_MAX_US     4000000LL
#define PH_PERIOD_NOM_US 60000000LL
#define PH_PERIOD_SPAN_US 1000000LL   /* nominal model allowance      */
#define PH_BR_MAX_US     15000000LL   /* wider bracket = reacquire    */
#define PH_PULLS_MAX            10
#define PH_REFINE_CYCLES         2
#define PH_CYCLE_LOOKAHEAD       8
#define PH_TARGET_WIDTH_US  400000LL  /* REFINE target, informational */
#define PH_INF             (INT64_MAX / 4)

enum {
    PH_RQ_NONE = 0,
    PH_RQ_MISS,
    PH_RQ_WIDE,
    PH_RQ_CONTRADICTION,
    PH_RQ_AMBIGUOUS,
    PH_RQ_BEACON,
    PH_RQ_SESSION,
    PH_RQ_DAY,
    PH_RQ_DISABLED,
};

static const struct { int64_t spacing; int min_pulls; } s_grid[] = {
    [PH_NARROW1]  = {2000000, 4},
    [PH_NARROW2]  = { 800000, 4},
    [PH_VERNIER]  = { 800000, 3},
    [PH_REFINE]   = { 500000, 3},     /* service guard may widen it */
    [PH_MAINTAIN] = {1000000, 3},
    [PH_REFINE2]  = { 500000, 3},     /* sustained refinement (u6)  */
};

const char *phase_state_name(ph_state_t st)
{
    static const char *n[] = {"watch", "hunt", "narrow1", "narrow2",
                              "vernier", "refine", "maintain",
                              "refine2"};
    return st >= 0 && st <= PH_REFINE2 ? n[st] : "?";
}

const char *phase_reacq_name(uint8_t reason)
{
    static const char *n[] = {"none", "miss", "wide_bracket",
                              "contradiction", "ambiguous_cycle",
                              "beacon_swap", "session_edge",
                              "day_change", "disabled"};
    return reason <= PH_RQ_DISABLED ? n[reason] : "?";
}

static void fit_reset(phase_t *p, uint8_t reason)
{
    p->n_obs = 0;
    p->base_cycle = 0;
    p->last_cycle = 0;
    p->phase_lo_us = 0;
    p->phase_hi_us = 0;
    p->per_lo_us = PH_PERIOD_NOM_US - PH_PERIOD_SPAN_US;
    p->per_hi_us = PH_PERIOD_NOM_US + PH_PERIOD_SPAN_US;
    p->period_us = PH_PERIOD_NOM_US;
    p->cyc_us = PH_CYC_INIT_US;
    p->pred_lo_us = p->pred_hi_us = 0;
    p->iunc_us = 0;
    p->mark_us = 0;
    p->fit_valid = false;
    p->cycle_ambig = false;
    p->armed = false;
    p->shift_idx = 0;
    p->refine_left = 0;
    if (reason != PH_RQ_NONE) {
        p->reacq_reason = reason;
    }
}

static void full_reset(phase_t *p, uint8_t reason)
{
    fit_reset(p, reason);
    p->have_unch = false;
    /* far-past sentinel: "never pulled" — due lands on now and 0 is
     * a legitimate submission time, so it cannot mark empty         */
    p->last_due_us = INT64_MIN / 2;
    p->pend_due_us = 0;
    p->pull_sent = p->pull_total = p->resp_got = 0;
    p->n_brackets = 0;
    p->n_misses = 0;
}

void phase_init(phase_t *p)
{
    memset(p, 0, sizeof(*p));
    p->cfg.watch_gap_us = 5000000;
    p->cfg.rate_ppm = 500;
    p->cfg.enabled = true;
    p->st = PH_WATCH;
    fit_reset(p, PH_RQ_NONE);
    p->last_due_us = INT64_MIN / 2;
}

void phase_config(phase_t *p, const phase_cfg_t *c)
{
    bool was = p->cfg.enabled;
    if (c) p->cfg = *c;
    if (was && !p->cfg.enabled) {
        /* disabling the model must not leave a stale armed deadline */
        p->armed = false;
        p->st = p->session ? PH_HUNT : PH_WATCH;
        p->last_due_us = INT64_MIN / 2;
        p->reacq_reason = PH_RQ_DISABLED;
    }
}

void phase_track(phase_t *p, const char *day, bool live)
{
    if (!day || !day[0]) {
        if (p->day[0]) full_reset(p, PH_RQ_DAY);
        p->day[0] = '\0';
        p->session = false;
        p->st = PH_WATCH;
        return;
    }
    if (strncmp(p->day, day, CAT_DAY_STR)) {
        full_reset(p, PH_RQ_DAY);
        memset(p->day, 0, sizeof(p->day));
        strncpy(p->day, day, CAT_DAY_STR - 1);
        p->st = PH_WATCH;
        p->session = false;
    }
    if (live && !p->session) {
        /* session opened: a WATCH bracket on the same day seeds the
         * new phase segment; otherwise HUNT keeps polling normally   */
        p->session = true;
        p->st = p->n_obs ? PH_NARROW1 : PH_HUNT;
        p->armed = false;
        p->last_due_us = INT64_MIN / 2;
    } else if (!live && p->session) {
        /* session over: the next session's flush phase is its own —
         * fitted constraints do not carry across (spec §5.2)          */
        fit_reset(p, PH_RQ_SESSION);
        p->session = false;
        p->st = PH_WATCH;
        p->have_unch = false;
        p->last_due_us = INT64_MIN / 2;
    }
}

void phase_reacquire(phase_t *p)
{
    /* beacon swap: session-relative phase evidence must not combine
     * across files (spec §5.2). The folder-level unchanged bound does
     * survive; the next changed tuple starts a fresh cycle segment.  */
    fit_reset(p, PH_RQ_BEACON);
    p->st = p->session ? PH_HUNT : PH_WATCH;
    p->last_due_us = INT64_MIN / 2;
}

/* ---------------- bounded interval model ---------------- */

static void period_prior(const phase_t *p, int64_t *lo, int64_t *hi)
{
    int64_t rate = PH_PERIOD_NOM_US * (int64_t)p->cfg.rate_ppm / 1000000;
    int64_t span = PH_PERIOD_SPAN_US + rate;
    *lo = PH_PERIOD_NOM_US - span;
    *hi = PH_PERIOD_NOM_US + span;
}

static int64_t div_floor_i64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}

static int64_t div_ceil_i64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) && ((a < 0) == (b < 0))) q++;
    return q;
}

/* Recompute the feasible phase/period region. Variables are
 * (period, phase_at_base_cycle); every retained bracket contributes
 *
 *     lo_i - J <= phase + cycle_i * period <= hi_i + J
 *
 * and the linear half-planes are clipped exactly. Keeping the joint
 * region preserves phase/period correlation, which a pair of separate
 * phase and period hulls would lose over long cycle offsets.          */
typedef struct { double p, o; } ph_pt_t;

static int clip_halfplane(ph_pt_t *v, int n, double a, double b,
                          double c)
{
    ph_pt_t out[2 * PH_OBS_CAP + 8];
    int m = 0;
    for (int i = 0; i < n; i++) {
        ph_pt_t cur = v[i], nxt = v[(i + 1) % n];
        double fc = a * cur.p + b * cur.o - c;
        double fn = a * nxt.p + b * nxt.o - c;
        double lim = 1e-7 * (fabs(c) > 1.0 ? fabs(c) : 1.0);
        bool in_c = fc <= lim, in_n = fn <= lim;
        if (in_c) {
            out[m++] = cur;
        }
        if (in_c != in_n) {
            double t = fc / (fc - fn);
            out[m++] = (ph_pt_t){
                .p = cur.p + t * (nxt.p - cur.p),
                .o = cur.o + t * (nxt.o - cur.o),
            };
        }
        if (m > (int)(sizeof(out) / sizeof(out[0]))) {
            return -1;
        }
    }
    memcpy(v, out, (size_t)m * sizeof(v[0]));
    return m;
}

static bool fit_recalc(phase_t *p)
{
    if (!p->n_obs) {
        p->fit_valid = false;
        return false;
    }
    int64_t plo, phi;
    period_prior(p, &plo, &phi);
    p->base_cycle = p->obs[0].cycle;

    ph_pt_t v[2 * PH_OBS_CAP + 8] = {
        {(double)plo, (double)(p->obs[0].lo_us - p->cyc_us)},
        {(double)phi, (double)(p->obs[0].lo_us - p->cyc_us)},
        {(double)phi, (double)(p->obs[0].hi_us + p->cyc_us)},
        {(double)plo, (double)(p->obs[0].hi_us + p->cyc_us)},
    };
    int nv = 4;
    for (int i = 0; i < p->n_obs && nv > 0; i++) {
        double k = (double)(p->obs[i].cycle - p->base_cycle);
        nv = clip_halfplane(v, nv, k, 1.0,
                            (double)p->obs[i].hi_us + p->cyc_us);
        if (nv <= 0) break;
        nv = clip_halfplane(v, nv, -k, -1.0,
                            -(double)p->obs[i].lo_us + p->cyc_us);
    }
    if (nv <= 0) return false;

    double qlo = v[0].p, qhi = v[0].p;
    double olo = v[0].o, ohi = v[0].o;
    for (int i = 1; i < nv; i++) {
        if (v[i].p < qlo) qlo = v[i].p;
        if (v[i].p > qhi) qhi = v[i].p;
        if (v[i].o < olo) olo = v[i].o;
        if (v[i].o > ohi) ohi = v[i].o;
    }
    p->per_lo_us = (int64_t)floor(qlo);
    p->per_hi_us = (int64_t)ceil(qhi);
    p->period_us = (p->per_lo_us + p->per_hi_us) / 2;
    p->phase_lo_us = (int64_t)floor(olo);
    p->phase_hi_us = (int64_t)ceil(ohi);
    p->last_cycle = p->obs[p->n_obs - 1].cycle;

    double ahead = (double)(p->last_cycle + 1 - p->base_cycle);
    double nlo = PH_INF, nhi = -PH_INF;
    for (int i = 0; i < nv; i++) {
        double t = v[i].o + ahead * v[i].p;
        if (t < nlo) nlo = t;
        if (t > nhi) nhi = t;
    }
    p->pred_lo_us = (int64_t)floor(nlo) - p->cyc_us;
    p->pred_hi_us = (int64_t)ceil(nhi) + p->cyc_us;
    p->mark_us = (p->pred_lo_us + p->pred_hi_us) / 2;
    p->iunc_us = (p->pred_hi_us - p->pred_lo_us) / 2;
    p->fit_valid = true;
    return true;
}

/* Assign the new bracket a unique cycle count relative to the newest
 * retained observation. A two-cycle gap is evidence at cycle+2, never
 * a 120-second fundamental period.                                   */
static bool cycle_assign(phase_t *p, int64_t lo, int64_t hi,
                         uint32_t *cycle)
{
    if (!p->n_obs) {
        *cycle = 0;
        return true;
    }
    const phase_obs_t *last = &p->obs[p->n_obs - 1];
    int64_t plo, phi;
    period_prior(p, &plo, &phi);
    int found = 0;
    uint32_t found_n = 0;
    for (uint32_t n = 1; n <= PH_CYCLE_LOOKAHEAD; n++) {
        int64_t dlo = lo - last->hi_us - 2 * p->cyc_us;
        int64_t dhi = hi - last->lo_us + 2 * p->cyc_us;
        int64_t lo_n = div_ceil_i64(dlo, (int64_t)n);
        int64_t hi_n = div_floor_i64(dhi, (int64_t)n);
        if (lo_n <= phi && hi_n >= plo) {
            found++;
            found_n = n;
        }
    }
    if (found != 1) {
        p->cycle_ambig = true;
        p->reacq_reason = PH_RQ_AMBIGUOUS;
        return false;
    }
    *cycle = last->cycle + found_n;
    p->cycle_ambig = false;
    return true;
}

static bool fit_add(phase_t *p, int64_t lo, int64_t hi)
{
    uint32_t cyc;
    if (!cycle_assign(p, lo, hi, &cyc)) {
        return false;
    }
    if (p->n_obs == PH_OBS_CAP) {
        /* bounded ring: expiring an old constraint is explicit — all
         * bounds are recomputed from what remains                    */
        memmove(p->obs, p->obs + 1,
                (PH_OBS_CAP - 1) * sizeof(p->obs[0]));
        p->n_obs--;
    }
    p->obs[p->n_obs++] = (phase_obs_t){
        .cycle = cyc, .lo_us = lo, .hi_us = hi,
    };
    for (int tries = 0; tries < 4; tries++) {
        if (fit_recalc(p)) return true;
        /* a bounded cycle surprise may be too optimistic — widen the
         * declared model before declaring a contradiction            */
        if (p->cyc_us >= PH_CYC_MAX_US) break;
        p->cyc_us *= 2;
        if (p->cyc_us > PH_CYC_MAX_US) p->cyc_us = PH_CYC_MAX_US;
    }
    return false;
}

/* ---------------- internal scheduler helpers ---------------- */

static int64_t grid_shift(const phase_t *p)
{
    if (p->st == PH_VERNIER) return 400000;
    if (p->st == PH_REFINE)  return 200000 * (p->shift_idx + 1);
    return 0;
}

static void to_continuous(phase_t *p, int64_t now_us);

/* Arm [pred_lo-margin, pred_hi+margin] on the state's shifted grid —
 * the cluster is centered on the current prediction and may move in
 * either direction cycle to cycle (never an accumulating one-way
 * shift; update6 §5.1). The service allowance ensures the first/last
 * response can still observe a transition just outside the
 * request-start interval.                                             */
static void win_arm(phase_t *p, int64_t svc_us, int64_t now_us)
{
    int64_t spacing = s_grid[p->st].spacing;
    if (svc_us > 0 && spacing < 2 * svc_us) {
        spacing = 2 * svc_us;          /* service-time guard §5       */
    }
    int64_t half_svc = svc_us > 0 ? svc_us / 2 : 0;
    int64_t shift = grid_shift(p);
    int64_t lo = p->pred_lo_us - PH_MARGIN_US - half_svc;
    int64_t hi = p->pred_hi_us + PH_MARGIN_US + half_svc;
    if (p->st == PH_MAINTAIN) {
        lo = p->mark_us - 1000000 - PH_MARGIN_US - half_svc;
        if (lo > p->pred_lo_us - PH_MARGIN_US - half_svc) {
            lo = p->pred_lo_us - PH_MARGIN_US - half_svc;
        }
        hi = p->mark_us + 1000000 + PH_MARGIN_US + half_svc;
        if (hi < p->pred_hi_us + PH_MARGIN_US + half_svc) {
            hi = p->pred_hi_us + PH_MARGIN_US + half_svc;
        }
    }
    p->grid0_us = lo + shift;
    p->grid_hi_us = hi + shift;
    int64_t span = p->grid_hi_us - p->grid0_us;
    int64_t n = span <= 0 ? 1 : (span + spacing - 1) / spacing + 1;
    if (n > PH_PULLS_MAX) {
        if (p->st == PH_REFINE2) {
            /* update6: coverage needing more than the pull cap falls
             * back to broad-window reacquisition — never shrink the
             * declared prediction or silently skip an edge.          */
            to_continuous(p, now_us);
            return;
        }
        /* the cap must not truncate coverage: widen the grid         */
        spacing = (span + PH_PULLS_MAX - 2) / (PH_PULLS_MAX - 1);
        spacing = (spacing + 99999) / 100000 * 100000;
        n = PH_PULLS_MAX;
    }
    if (n < s_grid[p->st].min_pulls) n = s_grid[p->st].min_pulls;
    p->spacing_us = spacing;
    p->pull_total = (int)n;
    p->pull_sent = 0;
    p->resp_got = 0;
    p->armed = true;
}

static void to_continuous(phase_t *p, int64_t now_us)
{
    p->armed = false;
    p->st = p->session ? PH_HUNT : PH_WATCH;
    /* resume polling immediately rather than one gap late           */
    p->last_due_us = now_us - (int64_t)p->cfg.watch_gap_us;
}

static void miss(phase_t *p, int64_t now_us)
{
    p->n_misses++;
    p->cyc_us *= 2;
    if (p->cyc_us < PH_CYC_INIT_US) p->cyc_us = PH_CYC_INIT_US;
    if (p->cyc_us > PH_CYC_MAX_US) p->cyc_us = PH_CYC_MAX_US;
    p->reacq_reason = PH_RQ_MISS;
    fit_recalc(p);                 /* keep the widened model for +k  */
    to_continuous(p, now_us);
}

/* ---------------- planner interface ---------------- */

bool phase_due(phase_t *p, int64_t now_us, int64_t svc_us,
               int64_t *due_us, int64_t *gap_us)
{
    if (!p->day[0] || !due_us || !gap_us) return false;

    if (p->st <= PH_HUNT || !p->cfg.enabled) {
        int64_t gap = p->cfg.watch_gap_us;
        int64_t due = p->last_due_us + gap;
        if (due < now_us - gap) {
            due = now_us;      /* first pull / stalled > one gap     */
        }
        p->pend_due_us = due;
        *due_us = due;
        *gap_us = 0;                    /* caller's normal spacing     */
        return true;
    }

    if (!p->fit_valid) {
        to_continuous(p, now_us);
        p->pend_due_us = now_us;
        *due_us = now_us;
        *gap_us = 0;
        return true;
    }

    if (!p->armed) {
        int64_t half_svc = svc_us > 0 ? svc_us / 2 : 0;
        int64_t arm_at = p->pred_lo_us - PH_MARGIN_US - half_svc
                       + grid_shift(p);
        if (now_us < arm_at) {
            p->pend_due_us = arm_at;
            *due_us = arm_at;
            *gap_us = 0;
            return true;
        }
        win_arm(p, svc_us, now_us);
        if (!p->armed) {
            /* coverage needed more than the pull cap: win_arm fell
             * back to continuous reacquisition — answer for that     */
            p->pend_due_us = now_us;
            *due_us = now_us;
            *gap_us = 0;
            return true;
        }
    }

    if (p->pull_sent < p->pull_total) {
        int64_t due = p->grid0_us + (int64_t)p->pull_sent * p->spacing_us;
        if (due < now_us - p->spacing_us) {
            due = now_us;               /* severe slip: fire now       */
        }
        p->pend_due_us = due;
        *due_us = due;
        *gap_us = p->spacing_us;
        return true;
    }

    /* all pulls submitted; feeds/fails settle the window. Past the
     * response deadline with no transition, it is a bounded miss.    */
    int64_t deadline = p->grid_hi_us + 2 * PH_MARGIN_US
                     + 2 * (svc_us > 0 ? svc_us : 0);
    if (now_us >= deadline) {
        miss(p, now_us);
        p->pend_due_us = now_us;
        *due_us = now_us;
        *gap_us = 0;
        return true;
    }
    p->pend_due_us = deadline;
    *due_us = deadline;
    *gap_us = 0;
    return true;
}

void phase_submitted(phase_t *p)
{
    if (p->armed && p->pull_sent < p->pull_total) {
        p->pull_sent++;
    } else {
        p->last_due_us = p->pend_due_us;
    }
}

/* ---------------- observation interface ---------------- */

void phase_obs(phase_t *p, int64_t req_us, int64_t resp_us,
               bool transition)
{
    if (!p->day[0]) return;

    if (!transition) {
        p->unch_req_us = req_us;
        p->have_unch = true;
        if (p->armed) {
            p->resp_got++;
            if (p->resp_got >= p->pull_total &&
                p->pull_sent >= p->pull_total) {
                miss(p, resp_us);       /* window closed quiet        */
            }
        }
        return;
    }

    /* one-sided stop: the rest of this cycle's window is cancelled  */
    p->armed = false;
    if (!p->have_unch || resp_us <= p->unch_req_us) {
        return;                         /* baseline / unbounded        */
    }
    int64_t lo = p->unch_req_us;
    int64_t hi = resp_us;
    int64_t w = hi - lo;
    p->have_unch = false;      /* next bound comes from a fresh scan */
    p->last_br_lo_us = lo;
    p->last_br_hi_us = hi;
    p->n_brackets++;

    if (w > PH_BR_MAX_US) {
        p->n_misses++;
        p->reacq_reason = PH_RQ_WIDE;
        p->st = p->session ? PH_HUNT : PH_WATCH;
        p->last_due_us = INT64_MIN / 2;
        return;
    }

    if (!fit_add(p, lo, hi)) {
        /* Ambiguous or contradictory constraints start a new phase
         * segment instead of silently averaging incompatible data.   */
        uint8_t why = p->cycle_ambig ? PH_RQ_AMBIGUOUS
                                   : PH_RQ_CONTRADICTION;
        fit_reset(p, why);
        p->obs[p->n_obs++] = (phase_obs_t){
            .cycle = 0, .lo_us = lo, .hi_us = hi,
        };
        fit_recalc(p);
        p->n_misses++;
        p->st = p->session ? PH_NARROW1 : PH_WATCH;
        p->last_due_us = INT64_MIN / 2;
        return;
    }

    /* a bracket the model accepted resolves the last invalidation   */
    if (p->cfg.enabled) p->reacq_reason = PH_RQ_NONE;

    if (p->session) {
        switch (p->st) {
        case PH_WATCH:
        case PH_HUNT:    p->st = PH_NARROW1; break;
        case PH_NARROW1: p->st = PH_NARROW2; break;
        case PH_NARROW2: p->st = PH_VERNIER; p->shift_idx = 0; break;
        case PH_VERNIER:
            if (p->cfg.refine) {
                p->st = PH_REFINE;
                p->refine_left = PH_REFINE_CYCLES;
                p->shift_idx = 0;
            } else {
                p->st = PH_MAINTAIN;
            }
            break;
        case PH_REFINE:
            if (--p->refine_left <= 0) {
                /* update6: sustained refinement — the tracker keeps
                 * polling the REFINE grid around each next-cycle
                 * prediction instead of coarsening to MAINTAIN.      */
                p->st = PH_REFINE2;
            } else {
                p->shift_idx++;
            }
            break;
        case PH_REFINE2:
            /* stay: the rolling estimator needs fresh brackets for the
             * full therapy duration; misses route through the miss
             * path (widen + reacquire) like every other state.        */
            break;
        case PH_MAINTAIN:
            if (p->cfg.refine &&
                p->phase_hi_us - p->phase_lo_us > 2 * PH_TARGET_WIDTH_US) {
                p->st = PH_REFINE;
                p->refine_left = 1;
                p->shift_idx = 0;
            }
            break;
        default: break;
        }
    }
}

void phase_fail(phase_t *p, int64_t now_us)
{
    if (!p->armed) return;
    p->resp_got++;
    if (p->resp_got >= p->pull_total && p->pull_sent >= p->pull_total) {
        miss(p, now_us);
    }
}
