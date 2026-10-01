/*
 * SomnoPal — phase-tracking poll schedule (update2 spec §3)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Tracks the recurring ~60 s visible-update phase of the tracked
 * DATALOG day folder and answers one question: when should the next
 * tracked-day listing be submitted? Pure logic, host-testable — all
 * times are caller-supplied monotonic microseconds; the tracker owns
 * no I/O and knows nothing about HTTP.
 *
 * One tracker per firmware instance, owned by the sched task. Feed it
 * every completed listing of the tracked folder (phase_obs/phase_fail)
 * and ask it for the next submission time (phase_due/phase_submitted).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "catalog.h"     /* CAT_DAY_STR */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PH_WATCH = 0,   /* no live estimator session: fixed watch-gap pulls */
    PH_HUNT,        /* session live, no bracket yet: same fixed pulls   */
    PH_NARROW1,     /* ladder: 2 s nominal grid                         */
    PH_NARROW2,     /*         0.8 s nominal grid                       */
    PH_VERNIER,     /*         0.8 s grid, +0.4 s cycle shift           */
    PH_REFINE,      /* optional: further shifted grids (profile-gated)  */
    PH_MAINTAIN,    /* steady: pulls centred on the predicted mark      */
} ph_state_t;

#define PH_MID_CAP 6    /* transition midpoints kept for the period fit */

typedef struct {
    uint32_t watch_gap_us;   /* continuous-poll spacing (5 s)          */
    uint32_t rate_ppm;       /* device/SomnoPal rate-disagreement bound */
    bool     enabled;        /* false pins the tracker to fixed polls  */
    bool     refine;         /* allow the optional REFINE stage        */
} phase_cfg_t;

typedef struct {
    ph_state_t st;
    char       day[CAT_DAY_STR];
    bool       session;         /* estimator session open on the day   */

    phase_cfg_t cfg;
    int64_t  last_due_us;       /* last scheduled continuous pull      */
    int64_t  pend_due_us;       /* due value last handed to the caller */

    /* observation: req_start of the newest UNCHANGED tracked scan —
     * the lower bound for the next transition bracket              */
    int64_t  unch_req_us;
    bool     have_unch;

    /* fitted phase (monotonic domain) */
    int64_t  mids[PH_MID_CAP];  /* recent transition bracket midpoints */
    int      n_mids;
    int64_t  period_us;
    int      n_period;
    int64_t  dpers[4];          /* recent valid inter-midpoint deltas  */
    int      n_dper;
    int64_t  resid_us[4];       /* |mid - predicted mark| samples      */
    int      n_resid;
    int64_t  cyc_us;            /* adaptive per-cycle surprise bound   */
    int64_t  iunc_us;           /* next-cycle prediction half-width    */
    int64_t  mark_us;           /* predicted next transition midpoint  */

    /* armed cycle window */
    bool     armed;
    int64_t  grid0_us;          /* nominal first-pull start            */
    int64_t  spacing_us;
    int      pull_total;
    int      pull_sent;
    int      resp_got;          /* feeds + fails inside the window     */
    int      shift_idx;         /* refine grid-shift step              */
    int      refine_left;

    /* stats for journal/API */
    int64_t  last_br_lo_us;
    int64_t  last_br_hi_us;
    uint32_t n_brackets;
    uint32_t n_misses;
} phase_t;

void phase_init(phase_t *p);
void phase_config(phase_t *p, const phase_cfg_t *c);

/* Track `day` (NULL/empty clears the target). `live` = an estimator
 * session is open on that day. Day changes reset everything; session
 * edges reset the fit (a new session's flush phase differs).         */
void phase_track(phase_t *p, const char *day, bool live);
void phase_reacquire(phase_t *p);   /* beacon swap: keep day, refit   */

/* Nominal start time of the next tracked-day pull. *gap_us is the
 * minimum inter-start spacing the executor should enforce for it
 * (0 -> caller's default). due may be in the future — submit at due.
 * Returns false when nothing is tracked.                           */
bool phase_due(phase_t *p, int64_t now_us, int64_t svc_us,
               int64_t *due_us, int64_t *gap_us);
void phase_submitted(phase_t *p);   /* call once per accepted submit  */

/* One completed tracked-day listing. `transition` = the tracked
 * update signal moved between the previous unchanged scan and this
 * response (the beacon tuple event, or a pre-session file change).   */
void phase_obs(phase_t *p, int64_t req_us, int64_t resp_us,
               bool transition);

/* A tracked-day pull failed — counts as a window response with no
 * observation so the miss path stays deterministic.                */
void phase_fail(phase_t *p, int64_t now_us);

const char *phase_state_name(ph_state_t st);

#ifdef __cplusplus
}
#endif
