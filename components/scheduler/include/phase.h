/*
 * SomnoBridge — phase-tracking poll schedule (update3 spec §4)
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
    PH_HUNT,        /* session live, no usable model: fixed pulls       */
    PH_NARROW1,     /* ladder: 2 s nominal grid                         */
    PH_NARROW2,     /*         0.8 s nominal grid                       */
    PH_VERNIER,     /*         shifted grid, interval constraints       */
    PH_REFINE,      /*         further shifted grids                    */
    PH_MAINTAIN,    /* steady: pulls around the bounded prediction      */
    PH_REFINE2,     /* sustained: REFINE grid every cycle, no drift     */
                    /* of the cluster — centered on each prediction    */
} ph_state_t;

#define PH_OBS_CAP 24   /* bounded transition brackets retained         */

typedef struct {
    uint32_t watch_gap_us;   /* continuous-poll spacing (5 s)          */
    uint32_t rate_ppm;       /* device/SomnoBridge rate-disagreement bound */
    bool     enabled;        /* false pins the tracker to fixed polls  */
    bool     refine;         /* allow the REFINE stage                  */
} phase_cfg_t;

typedef struct {
    uint32_t cycle;          /* fitted cycle index, not listing count  */
    int64_t  lo_us;          /* previous unchanged request start       */
    int64_t  hi_us;          /* changed response end                   */
} phase_obs_t;

typedef struct {
    ph_state_t st;
    char       day[CAT_DAY_STR];
    bool       session;         /* estimator session open on the day   */

    phase_cfg_t cfg;
    int64_t  last_due_us;       /* last scheduled continuous pull      */
    int64_t  pend_due_us;       /* due value last handed to the caller */

    /* observation lower bound: req_start of the newest UNCHANGED
     * tracked scan, in the same beacon/session phase segment          */
    int64_t  unch_req_us;
    bool     have_unch;

    /* bounded phase/period model. phase bounds are re-referenced to
     * base_cycle; prediction projects them to last_cycle+1.           */
    phase_obs_t obs[PH_OBS_CAP];
    int      n_obs;
    uint32_t base_cycle;
    uint32_t last_cycle;
    int64_t  phase_lo_us, phase_hi_us;
    int64_t  per_lo_us, per_hi_us;
    int64_t  period_us;         /* midpoint, diagnostics only          */
    int64_t  cyc_us;            /* per-cycle visible variation bound   */
    int64_t  pred_lo_us, pred_hi_us;
    int64_t  iunc_us;           /* prediction half-width               */
    int64_t  mark_us;           /* prediction midpoint                 */
    bool     fit_valid;
    bool     cycle_ambig;
    uint8_t  reacq_reason;      /* recovery cause until accepted fit   */

    /* armed cycle window */
    bool     armed;
    int64_t  grid0_us;          /* nominal first-pull start            */
    int64_t  grid_hi_us;        /* nominal last covered instant        */
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
const char *phase_reacq_name(uint8_t reason);

#ifdef __cplusplus
}
#endif
