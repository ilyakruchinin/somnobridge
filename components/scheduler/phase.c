/*
 * SomnoPal — phase-tracking poll schedule (update2 spec §3)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * WATCH/HUNT pull on a fixed gap; once a transition is bracketed the
 * ladder narrows the grid around the predicted next-update mark, an
 * optional shifted-grid refinement may follow, then MAINTAIN runs a
 * small bracketing set per cycle. A miss (predicted window closes
 * quiet) or an unbracketed/oversized observation returns to HUNT —
 * never silently widens into a claimed precision.
 */
#include <string.h>

#include "phase.h"

/* -------- constants (spec §3.3 nominal grids + guards) -------- */
#define PH_MARGIN_US      500000LL   /* window edge allowance        */
#define PH_CYC_INIT_US    1500000LL  /* pre-fit cycle surprise bound */
#define PH_CYC_MIN_US      250000LL
#define PH_CYC_MAX_US     4000000LL
#define PH_PERIOD_DFLT_US 60000000LL /* nominal 60 s recurrence      */
#define PH_PERIOD_MIN_US  30000000LL
#define PH_PERIOD_MAX_US 180000000LL
#define PH_BR_MAX_US      15000000LL /* wider bracket = reacquire    */
#define PH_PULLS_MAX      10         /* per-cycle submission cap     */
#define PH_REFINE_CYCLES  2

static const struct { int64_t spacing; int min_pulls; } s_grid[] = {
    [PH_NARROW1]  = {2000000, 4},
    [PH_NARROW2]  = { 800000, 4},
    [PH_VERNIER]  = { 800000, 3},
    [PH_REFINE]   = { 800000, 3},
    [PH_MAINTAIN] = {1000000, 3},
};

const char *phase_state_name(ph_state_t st)
{
    static const char *n[] = {"watch", "hunt", "narrow1", "narrow2",
                              "vernier", "refine", "maintain"};
    return st >= 0 && st <= PH_MAINTAIN ? n[st] : "?";
}

static void fit_reset(phase_t *p)
{
    p->n_mids = 0;
    p->period_us = PH_PERIOD_DFLT_US;
    p->n_period = 0;
    p->n_resid = 0;
    p->cyc_us = PH_CYC_INIT_US;
    p->iunc_us = 0;
    p->mark_us = 0;
    p->armed = false;
    p->shift_idx = 0;
    p->refine_left = 0;
}

static void full_reset(phase_t *p)
{
    fit_reset(p);
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
    fit_reset(p);
}

void phase_config(phase_t *p, const phase_cfg_t *c)
{
    if (c) p->cfg = *c;
}

void phase_track(phase_t *p, const char *day, bool live)
{
    if (!day || !day[0]) {
        if (p->day[0]) full_reset(p);
        p->day[0] = '\0';
        p->session = false;
        p->st = PH_WATCH;
        return;
    }
    if (strncmp(p->day, day, CAT_DAY_STR)) {
        full_reset(p);
        memset(p->day, 0, sizeof(p->day));
        strncpy(p->day, day, CAT_DAY_STR - 1);
        p->st = PH_WATCH;
        p->session = false;
    }
    if (live && !p->session) {
        /* session opened on the tracked day: enter the ladder; a
         * bracket already taken while watching seeds the first
         * prediction (it is this session's own write activity)      */
        p->session = true;
        p->st = p->n_mids ? PH_NARROW1 : PH_HUNT;
        p->armed = false;
        p->last_due_us = INT64_MIN / 2;   /* hunt starts immediately */
    } else if (!live && p->session) {
        /* session over: the next session's flush phase is its own —
         * the fitted evidence does not carry across (spec §3.5)      */
        fit_reset(p);
        p->session = false;
        p->st = PH_WATCH;
        p->last_due_us = INT64_MIN / 2;
    }
}

void phase_reacquire(phase_t *p)
{
    /* beacon swap: session-relative phase evidence must not combine
     * across files (spec §3.5). The unchanged-scan bound survives —
     * it is folder-level, not beacon-level.                        */
    fit_reset(p);
    p->st = p->session ? PH_HUNT : PH_WATCH;
    p->last_due_us = INT64_MIN / 2;
}

/* ---------------- internal helpers ---------------- */

static int64_t grid_shift(const phase_t *p)
{
    if (p->st == PH_VERNIER) return 400000;
    if (p->st == PH_REFINE)  return 200000 * (p->shift_idx + 1);
    return 0;
}

/* Half-width of the interval the next window must cover:
 * prediction uncertainty, the state's nominal half-span (MAINTAIN
 * always covers mark +/-1 s), the edge margin, and a service-time
 * allowance so the first/last pull still bracket the edges.        */
static int64_t half_cover(const phase_t *p, int64_t svc_us)
{
    int64_t half = p->iunc_us;
    if (p->st == PH_MAINTAIN && half < 1000000) half = 1000000;
    half += PH_MARGIN_US + (svc_us > 0 ? svc_us / 2 : 0);
    return half;
}

static void win_arm(phase_t *p, int64_t svc_us)
{
    int64_t spacing = s_grid[p->st].spacing;
    int64_t half = half_cover(p, svc_us);
    int64_t shift = grid_shift(p);
    p->grid0_us = p->mark_us - half + shift;
    p->spacing_us = spacing;
    /* cover [mark-half, mark+half] on the shifted lattice: the grid
     * start moved right by `shift`, so the span grows accordingly    */
    int64_t span = 2 * half + (shift > 0 ? shift : 0);
    int64_t n = (span + spacing - 1) / spacing + 1;
    if (n < s_grid[p->st].min_pulls) n = s_grid[p->st].min_pulls;
    if (n > PH_PULLS_MAX) n = PH_PULLS_MAX;
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
    to_continuous(p, now_us);
}

/* ---------------- planner interface ---------------- */

bool phase_due(phase_t *p, int64_t now_us, int64_t svc_us,
               int64_t *due_us, int64_t *gap_us)
{
    if (!p->day[0]) return false;

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

    if (!p->mark_us) {
        /* ladder state without a prediction is meaningless — refit  */
        to_continuous(p, now_us);
        p->pend_due_us = now_us;
        *due_us = now_us;
        *gap_us = 0;
        return true;
    }

    if (!p->armed) {
        int64_t arm_at = p->mark_us - half_cover(p, svc_us)
                       + grid_shift(p);
        if (now_us < arm_at) {
            p->pend_due_us = arm_at;
            *due_us = arm_at;
            *gap_us = 0;
            return true;
        }
        win_arm(p, svc_us);
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

    /* all pulls submitted; responses settle via phase_obs/phase_fail.
     * Past the deadline with no transition, the window is a miss.    */
    int64_t deadline = p->grid0_us + (int64_t)p->pull_total * p->spacing_us
                     + 2 * PH_MARGIN_US + 2 * (svc_us > 0 ? svc_us : 0);
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

    /* Observed change — one-sided stop: the rest of this window is
     * cancelled; the bracket feeds the phase fit.                   */
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
        /* the transition landed far outside the predicted window —
         * a phase inconsistency, not a usable bracket (spec §3.4)    */
        p->n_misses++;
        p->cyc_us *= 2;
        if (p->cyc_us < PH_CYC_INIT_US) p->cyc_us = PH_CYC_INIT_US;
        if (p->cyc_us > PH_CYC_MAX_US) p->cyc_us = PH_CYC_MAX_US;
        p->st = p->session ? PH_HUNT : PH_WATCH;
        p->last_due_us = INT64_MIN / 2;
        return;
    }

    int64_t mid = lo + w / 2;
    if (p->n_mids) {
        int64_t dper = mid - p->mids[p->n_mids - 1];
        if (dper >= PH_PERIOD_MIN_US && dper <= PH_PERIOD_MAX_US) {
            p->n_period++;
            p->period_us += (dper - p->period_us) / p->n_period;
        }
        if (p->mark_us) {
            int64_t err = mid - p->mark_us;
            if (err < 0) err = -err;
            if (p->n_resid < (int)(sizeof(p->resid_us) /
                                   sizeof(p->resid_us[0]))) {
                p->resid_us[p->n_resid++] = err;
            } else {
                memmove(p->resid_us, p->resid_us + 1,
                        3 * sizeof(p->resid_us[0]));
                p->resid_us[3] = err;
            }
            int64_t mx = err;
            for (int i = 0; i < p->n_resid; i++) {
                if (p->resid_us[i] > mx) mx = p->resid_us[i];
            }
            if (mx < PH_CYC_MIN_US) mx = PH_CYC_MIN_US;
            if (mx > PH_CYC_MAX_US) mx = PH_CYC_MAX_US;
            p->cyc_us = mx;
        }
    }
    if (p->n_mids < PH_MID_CAP) {
        p->mids[p->n_mids++] = mid;
    } else {
        memmove(p->mids, p->mids + 1,
                (PH_MID_CAP - 1) * sizeof(p->mids[0]));
        p->mids[PH_MID_CAP - 1] = mid;
    }
    /* next-cycle prediction uncertainty: this bracket's detection
     * half-width + adaptive cycle surprise + rate disagreement over
     * one period (spec §2 — iunc carries no automatic stamp floor)   */
    p->iunc_us = w / 2 + p->cyc_us
               + p->period_us * (int64_t)p->cfg.rate_ppm / 1000000;
    p->mark_us = mid + p->period_us;

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
                p->st = PH_MAINTAIN;
            } else {
                p->shift_idx++;
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
